#include "edt_editor_camera.h"
#include <snk/resource/res_tag.h>
#include <snk/scene/scn_camera_component.hpp>
#include <snk/scene/scn_camera_controller_component.hpp>
#include <snk/common/eng_transform_3d.hpp>
#include <snk/scene/scn_model.h>
#include <snk/scene/level/scn_world.h>
#include <entt/entt.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

namespace edt {

void save_camera_state(entt::registry& reg, editor_camera_state& state)
{
	auto view = reg.view<editor_camera_tag, scn::mouse_controller_component>();
	for (auto ent : view) {
		auto& ctrl = reg.get<scn::mouse_controller_component>(ent);
		state.position = ctrl.position;
		state.rotation = ctrl.rotation;
		state.distance = ctrl.distance;
		break;
	}
}

void inject_camera(entt::registry& reg, const editor_camera_state& state)
{
	for (auto ent : reg.view<editor_camera_tag>())
		reg.destroy(ent);

	entt::entity cam = reg.create();
	reg.emplace<editor_camera_tag>(cam);

	scn::camera_component cam_comp;
	cam_comp.fov           = state.fov;
	cam_comp.near_distance = state.near_distance;
	cam_comp.far_distance  = state.far_distance;
	cam_comp.color_targets.push_back(res::tag{ editor_camera_state::RT_TAG });
	cam_comp.depth_target = res::tag(res::tag::memory, "__editor_depth_rt");
	cam_comp.tp_accum_target = res::tag(res::tag::memory, "__editor_tp_accum_rt");
	cam_comp.tp_reveal_target = res::tag(res::tag::memory, "__editor_tp_reveal_rt");
	reg.emplace<scn::camera_component>(cam, cam_comp);

	scn::mouse_controller_component ctrl;
	ctrl.position       = state.position;
	ctrl.rotation       = state.rotation;
	ctrl.distance       = state.distance;
	ctrl.movement_speed = state.movement_speed;
	ctrl.rotating_speed = state.rotating_speed;
	reg.emplace<scn::mouse_controller_component>(cam, ctrl);

	glm::mat4 orientation = glm::toMat4(glm::quat(ctrl.rotation));
	glm::mat4 cam_matrix = glm::translate(glm::mat4(1.0f), ctrl.position)
		* orientation
		* glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, ctrl.distance));
	reg.emplace<scn::local_transform>(cam, scn::local_transform{ cam_matrix });
	reg.emplace<scn::world_transform>(cam);
	reg.emplace<scn::name_component>(cam, "Editor Camera");
	reg.emplace<scn::depth_level>(cam);
}

} // namespace edt
