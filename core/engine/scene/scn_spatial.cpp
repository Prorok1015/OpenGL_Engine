#include <snk/scene/scn_spatial.h>
#include "scn_spatial_detail.h"
#include <snk/scene/level/scn_screen_anchor.h>
#include <snk/scene/scn_camera_component.hpp>
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/common/engine_assert.h>
#include <snk/scene/scn_model.h>
#include <entt/entity/registry.hpp>
#include <algorithm>
#include <cmath>
#include <utility>

scn::spatial_world::spatial_world() = default;
scn::spatial_world::~spatial_world() = default;

void scn::spatial_world::on_structure_changed(entt::registry& world, entt::entity)
{
	if (auto* spatial = world.ctx().find<spatial_world>()) {
		spatial->m_backend->structure_changed();
	}
}

void scn::spatial_world::on_state_changed(entt::registry& world, entt::entity)
{
	if (auto* spatial = world.ctx().find<spatial_world>()) {
		spatial->m_backend->state_changed();
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
	world.on_construct<renderable>().connect<&spatial_world::on_state_changed>();
	world.on_destroy<renderable>().connect<&spatial_world::on_state_changed>();
	world.on_construct<spatial_layers>().connect<&spatial_world::on_state_changed>();
	world.on_update<spatial_layers>().connect<&spatial_world::on_state_changed>();
	world.on_destroy<spatial_layers>().connect<&spatial_world::on_state_changed>();
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

ds::task_handle<scn::ray_batch> scn::spatial_world::raycast_batch(ds::task_system& tasks, entt::registry& world,
	std::vector<ray_request> requests, std::uint64_t frame)
{
	ASSERT_MSG(!ds::task_system::on_worker(), "a batch is handed in on the main thread");
	return m_backend->raycast_batch(tasks, world, std::move(requests), frame);
}

void scn::spatial_world::overlap_frustum(entt::registry& world, const glm::mat4& view_projection, const ray_filter& filter,
	std::vector<entt::entity>& out)
{
	ASSERT_MSG(!ds::task_system::on_worker(), "spatial queries run on the main thread");
	const ds::frustum view = ds::frustum_from(view_projection);
	// A plane with no normal is no plane, but not always a broken matrix: with a far plane at infinity or a tiny near plane the near or
	// far one comes out as (0, 0, 0, w) in float, and w >= 0 means everything is on its inner side (ds::test_frustum takes it so). The
	// four sides must be real planes: they are what the all-zero matrix of an empty rectangle lacks.
	for (std::size_t i = 0; i < view.planes.size(); ++i) {
		const glm::vec4& plane = view.planes[i];
		const glm::vec3 normal{ plane };
		const bool has_normal = glm::dot(normal, normal) > 0.f;
		if (!std::isfinite(plane.x) || !std::isfinite(plane.y) || !std::isfinite(plane.z) || !std::isfinite(plane.w)
			|| (!has_normal && (i < 4 || plane.w < 0.f))) {
			return;
		}
	}
	m_backend->overlap_frustum(world, view, filter, out);
}

void scn::spatial_world::entities_moved(entt::registry& world, std::span<const entt::entity> moved)
{
	ASSERT_MSG(!ds::task_system::on_worker(), "entities_moved runs on the main thread");
	m_backend->entities_moved(world, moved);
}

ds::task_handle<scn::ray_batch> scn::raycast_batch(ds::task_system& tasks, entt::registry& world, std::vector<ray_request> requests,
	std::uint64_t frame)
{
	return spatial_world::attach(world).raycast_batch(tasks, world, std::move(requests), frame);
}

void scn::overlap_frustum(entt::registry& world, const glm::mat4& view_projection, const ray_filter& filter, std::vector<entt::entity>& out)
{
	spatial_world::attach(world).overlap_frustum(world, view_projection, filter, out);
}

glm::mat4 scn::screen_rect_view_projection(const glm::mat4& view, const glm::mat4& projection, glm::vec2 corner_a, glm::vec2 corner_b,
	glm::vec2 image_size)
{
	const glm::vec2 low = glm::min(corner_a, corner_b);
	const glm::vec2 high = glm::max(corner_a, corner_b);
	if (!(image_size.x > 0.f) || !(image_size.y > 0.f) || !(high.x > low.x) || !(high.y > low.y)) {
		return glm::mat4{ 0.f };
	}
	// The rectangle in clip space (y up), mapped back onto the whole clip square.
	const float left = 2.f * low.x / image_size.x - 1.f;
	const float right = 2.f * high.x / image_size.x - 1.f;
	const float bottom = 1.f - 2.f * high.y / image_size.y;
	const float top = 1.f - 2.f * low.y / image_size.y;
	glm::mat4 crop{ 1.f };
	crop[0][0] = 2.f / (right - left);
	crop[3][0] = -(right + left) / (right - left);
	crop[1][1] = 2.f / (top - bottom);
	crop[3][1] = -(top + bottom) / (top - bottom);
	return crop * projection * view;
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
