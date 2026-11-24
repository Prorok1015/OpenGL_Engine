#include <snk/scene/scn_spatial.h>
#include "scn_spatial_detail.h"
#include <snk/scene/level/scn_screen_anchor.h>
#include <snk/scene/scn_camera_component.hpp>
#include <snk/scene/scn_model.h>
#include <entt/entity/registry.hpp>
#include <algorithm>
#include <utility>

scn::spatial_world::spatial_world() = default;
scn::spatial_world::~spatial_world() = default;

scn::spatial_world& scn::spatial_world::attach(entt::registry& world)
{
	if (auto* existing = world.ctx().find<spatial_world>()) {
		return *existing;
	}
	auto& made = world.ctx().emplace<spatial_world>();
	made.add_backend(detail::make_mesh_backend());
	return made;
}

scn::spatial_world* scn::spatial_world::find(entt::registry& world)
{
	return world.ctx().find<spatial_world>();
}

void scn::spatial_world::add_backend(std::unique_ptr<spatial_backend> backend)
{
	if (backend) {
		m_backends.push_back(std::move(backend));
	}
}

std::size_t scn::spatial_world::backend_count() const
{
	return m_backends.size();
}

std::optional<scn::ray_hit> scn::spatial_world::raycast_closest(entt::registry& world, const ray& line, const ray_filter& filter)
{
	std::optional<ray_hit> best;
	// The next backend only looks as far as the best hit so far.
	ray_filter narrowed = filter;
	for (const auto& backend : m_backends) {
		if (auto hit = backend->raycast_closest(world, line, narrowed)) {
			if (!best || hit->distance < best->distance) {
				best = *hit;
				narrowed.max_distance = hit->distance;
			}
		}
	}
	return best;
}

bool scn::spatial_world::raycast_any(entt::registry& world, const ray& line, const ray_filter& filter)
{
	return std::ranges::any_of(m_backends, [&](const auto& backend) { return backend->raycast_any(world, line, filter); });
}

void scn::spatial_world::raycast_all(entt::registry& world, const ray& line, const ray_filter& filter, std::vector<ray_hit>& out)
{
	const std::size_t first = out.size();
	for (const auto& backend : m_backends) {
		backend->raycast_all(world, line, filter, out);
	}
	if (m_backends.size() > 1) {
		std::stable_sort(out.begin() + static_cast<std::ptrdiff_t>(first), out.end(),
			[](const ray_hit& a, const ray_hit& b) { return a.distance < b.distance; });
	}
}

void scn::spatial_world::entities_moved(entt::registry& world, std::span<const entt::entity> moved)
{
	for (const auto& backend : m_backends) {
		backend->entities_moved(world, moved);
	}
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
