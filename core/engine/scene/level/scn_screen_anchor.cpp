#include <snk/scene/level/scn_screen_anchor.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_context_set.h>
#include <snk/scene/level/scn_node_index.h>
#include <snk/scene/level/scn_world_context.h>
#include <snk/scene/scn_model.h>
#include <snk/render/rnd_render_packet.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <entt/entt.hpp>

namespace {
	struct camera_pick
	{
		const scn::camera_component* camera = nullptr;
		entt::entity entity = entt::null;
		glm::ivec4 viewport{ 0 };
	};

	// The camera of a registry that renders this frame: it has a viewport (its own, or the backbuffer for a
	// backbuffer camera). Lowest layer first; equal layers keep the storage order.
	camera_pick pick_camera(entt::registry& reg, const glm::ivec2& backbuffer_size)
	{
		camera_pick best;
		for (auto [ent, cam] : reg.view<scn::camera_component>().each()) {
			const auto viewport = rnd::effective_camera_viewport(
				cam.m_viewport, rnd::targets_backbuffer(cam.color_targets), backbuffer_size);
			if (!viewport) continue;
			if (!best.camera || cam.layer < best.camera->layer) {
				best = { &cam, ent, *viewport };
			}
		}
		return best;
	}

	scn::screen_anchor_result resolve_one(scn::context_set& set, scn::world_context& holder,
										  const scn::screen_anchor_component& anchor, const glm::ivec2& backbuffer_size)
	{
		scn::world_context* target_context = anchor.target.context.empty() ? &holder : set.find(anchor.target.context);
		if (!target_context || anchor.target.path.empty()) return {};

		auto& target_reg = target_context->state();
		const entt::entity target = scn::node_index::get(target_reg).find(anchor.target.path);
		if (target == entt::null) return {};
		const auto* transform = target_reg.try_get<scn::world_transform>(target);
		if (!transform) return {};

		const camera_pick pick = pick_camera(target_reg, backbuffer_size);
		if (!pick.camera) return {};

		// The same view the extractor gives the camera's packet: the inverse of its world transform.
		glm::mat4 view{ 1.f };
		if (const auto* camera_transform = target_reg.try_get<scn::world_transform>(pick.entity)) {
			view = glm::inverse(camera_transform->world);
		}
		const glm::vec3 position = glm::vec3(transform->world[3]) + anchor.offset;
		return scn::project_to_screen(view, scn::camera_projection_matrix(*pick.camera, pick.viewport), pick.viewport, position);
	}
}

glm::mat4 scn::camera_projection_matrix(const camera_component& cam, const glm::ivec4& viewport)
{
	const float aspect = static_cast<float>(viewport.z) / static_cast<float>(viewport.w);
	if (cam.projection == camera_projection::ortho) {
		const float half_height = cam.ortho_size * 0.5f;
		const float half_width = half_height * aspect;
		return glm::ortho(-half_width, half_width, -half_height, half_height, cam.near_distance, cam.far_distance);
	}
	return glm::perspective(glm::radians(cam.fov), aspect, cam.near_distance, cam.far_distance);
}

scn::screen_anchor_result scn::project_to_screen(const glm::mat4& view, const glm::mat4& projection,
												 const glm::ivec4& viewport, const glm::vec3& world_position)
{
	screen_anchor_result result;
	result.resolved = true;

	const glm::vec4 clip = projection * view * glm::vec4(world_position, 1.f);
	// Perspective: w is the distance in front of the camera. Orthographic: w is 1 and only the depth range tells.
	if (clip.w <= 0.f) {
		result.behind_camera = true;
		return result;
	}
	const glm::vec3 ndc = glm::vec3(clip) / clip.w;
	if (ndc.z < -1.f) {
		result.behind_camera = true;
		return result;
	}

	result.normalized = { ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f };
	result.position = result.normalized * glm::vec2(static_cast<float>(viewport.z), static_cast<float>(viewport.w));
	result.off_screen = ndc.z > 1.f || ndc.x < -1.f || ndc.x > 1.f || ndc.y < -1.f || ndc.y > 1.f;
	return result;
}

void scn::resolve_screen_anchors(context_set& set, const glm::ivec2& backbuffer_size)
{
	set.for_each([&](world_context& holder) {
		if (!holder.extract()) return;
		auto& reg = holder.state();
		for (auto [ent, anchor] : reg.view<screen_anchor_component>().each()) {
			reg.emplace_or_replace<screen_anchor_result>(ent, resolve_one(set, holder, anchor, backbuffer_size));
		}
	});
}

void scn::resolve_screen_anchors(context_manager& contexts, const glm::ivec2& backbuffer_size)
{
	contexts.for_each_set([&](context_set& set) { resolve_screen_anchors(set, backbuffer_size); });
}
