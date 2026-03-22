#include "scn_animation_job.h"
#include "scn_mesh_nodes.hpp"
#include "scn_model.h"
#include "ecs_event.hpp"
#include "engine_log.h"
#include <algorithm>

void calc_interpolated_scaling(glm::vec3& Out, float AnimationTimeTicks, const scn::animation_node& anim);
void calc_interpolated_position(glm::vec3& Out, float AnimationTimeTicks, const scn::animation_node& anim);
void calc_interpolated_rotation(glm::quat& Out, float AnimationTimeTicks, const scn::animation_node& anim);

static entt::entity find_skeleton_root(entt::registry& reg, entt::entity ent)
{
    entt::entity current = ent;
    while (auto* pc = reg.try_get<scn::parent_component>(current)) {
        if (reg.all_of<scn::scene_anchor_component>(pc->parent)) break;
        current = pc->parent;
    }
    return current;
}

void update_bone_offsets_system(
    entt::registry& reg,
    entt::view<entt::get_t<const scn::bone_component, const scn::world_transform>> bones_view
)
{
    for (auto&& [ent, bone, transform] : bones_view.each()) {
        entt::entity skeleton_root = find_skeleton_root(reg, ent);
        if (auto* matrices_comp = reg.try_get<scn::bone_matrices_component>(skeleton_root)) {
            if (bone.index >= 0 && bone.index < (int)matrices_comp->matrices.size()) {
                matrices_comp->matrices[bone.index] = transform.world * bone.offset;
            }
        }
    }
}

void update_animation_controllers_system(
    scn::level_res<const scn::delta_time> dt,
    entt::view<entt::get_t<scn::animation_controller_component>> controllers)
{
    for (auto [ent, ctrl] : controllers.each()) {
        if (!ctrl.playing) continue;
        ctrl.current_time += dt->dt * ctrl.speed;
        const float tps = ctrl.ticks_per_second > 0 ? ctrl.ticks_per_second : 25.0f;
        const float time_in_ticks = ctrl.current_time * tps;
        if (time_in_ticks > ctrl.duration) {
            if (ctrl.loop) {
                ctrl.current_time = 0.f;
            } else {
                ctrl.current_time = ctrl.duration / tps;
                ctrl.playing = false;
            }
        }
    }
}

void update_nodes_animation_system(
    entt::registry& reg,
    entt::view<entt::get_t<const scn::keyframes_component>> bones_view,
    ecs::event<scn::transform_updated>& event)
{
    for (auto [ent, keyframes] : bones_view.each()) {
        entt::entity root = find_skeleton_root(reg, ent);
        auto* ctrl = reg.try_get<scn::animation_controller_component>(root);
        if (!ctrl) continue;
        if (!ctrl->playing) continue;

        const float tps = ctrl->ticks_per_second > 0 ? ctrl->ticks_per_second : 25.0f;
        float ticks = std::fmod(ctrl->current_time * tps, ctrl->duration);

        auto& keys = keyframes.keyframes;
        if (auto it = keys.find(ctrl->current_animation); it != keys.end()) {
            glm::vec3 scaling;
            calc_interpolated_scaling(scaling, ticks, it->second);
            glm::mat4 scaling_m = glm::scale(scaling);

            glm::quat rotation_q;
            calc_interpolated_rotation(rotation_q, ticks, it->second);
            glm::mat4 rotation_m = glm::toMat4(rotation_q);

            glm::vec3 translation;
            calc_interpolated_position(translation, ticks, it->second);
            glm::mat4 translation_m = glm::translate(translation);

            auto local = translation_m * rotation_m * scaling_m;
            reg.get_or_emplace<scn::local_transform>(ent).local = local;
            event.emit(ent);
        }
    }
}
template<class T>
std::size_t find_keyframe_index(const std::vector<T>& arr, float time)
{
    if (arr.size() < 2) return 0;

    // Binary search: find the last key where key.time <= time
    auto it = std::upper_bound(arr.begin(), arr.end(), time,
        [](float t, const T& key) { return t < key.time; });

    if (it == arr.begin()) return 0;
    std::size_t idx = static_cast<std::size_t>(std::distance(arr.begin(), it)) - 1;
    // Clamp to second-to-last to ensure idx+1 is valid
    if (idx >= arr.size() - 1) idx = arr.size() - 2;
    return idx;
}

void calc_interpolated_scaling(glm::vec3& Out, float AnimationTimeTicks, const scn::animation_node& anim)
{
    if (anim.scale_keys.empty()) { Out = glm::vec3{1.0f}; return; }
    if (anim.scale_keys.size() == 1) {
        Out = anim.scale_keys[0].value;
        return;
    }

    std::size_t ScalingIndex = find_keyframe_index(anim.scale_keys, AnimationTimeTicks);
    std::size_t NextScalingIndex = ScalingIndex + 1;

    float t1 = anim.scale_keys[ScalingIndex].time;
    float t2 = anim.scale_keys[NextScalingIndex].time;
    float DeltaTime = t2 - t1;
    float Factor = std::clamp((AnimationTimeTicks - t1) / DeltaTime, 0.0f, 1.f);
    assert(Factor >= 0.0f && Factor <= 1.0f);
    const glm::vec3& Start = anim.scale_keys[ScalingIndex].value;
    const glm::vec3& End = anim.scale_keys[NextScalingIndex].value;
    glm::vec3 Delta = End - Start;
    Out = Start + Factor * Delta;
}

void calc_interpolated_position(glm::vec3& Out, float AnimationTimeTicks, const scn::animation_node& anim)
{
    if (anim.pos_keys.empty()) { Out = glm::vec3{0.0f}; return; }
    if (anim.pos_keys.size() == 1) {
        Out = anim.pos_keys[0].value;
        return;
    }

    std::size_t PositionIndex = find_keyframe_index(anim.pos_keys, AnimationTimeTicks);
    std::size_t NextPositionIndex = PositionIndex + 1;

    float t1 = (float)anim.pos_keys[PositionIndex].time;
    float t2 = (float)anim.pos_keys[NextPositionIndex].time;
    float DeltaTime = t2 - t1;
    float Factor = std::clamp((AnimationTimeTicks - t1) / DeltaTime, 0.0f, 1.f);

    const glm::vec3& Start = anim.pos_keys[PositionIndex].value;
    const glm::vec3& End = anim.pos_keys[NextPositionIndex].value;
    glm::vec3 Delta = End - Start;
    Out = Start + Factor * Delta;
}

void calc_interpolated_rotation(glm::quat& Out, float AnimationTimeTicks, const scn::animation_node& anim)
{
    if (anim.rotate_keys.empty()) { Out = glm::quat{1.0f, 0.0f, 0.0f, 0.0f}; return; }
    if (anim.rotate_keys.size() == 1) {
        Out = anim.rotate_keys[0].value;
        return;
    }

    std::size_t RotationIndex = find_keyframe_index(anim.rotate_keys, AnimationTimeTicks);
    std::size_t NextRotationIndex = RotationIndex + 1;

    float t1 = (float)anim.rotate_keys[RotationIndex].time;
    float t2 = (float)anim.rotate_keys[NextRotationIndex].time;
    float DeltaTime = t2 - t1;
    float Factor = std::clamp((AnimationTimeTicks - t1) / DeltaTime, 0.0f, 1.f);

    const glm::quat& StartRotationQ = anim.rotate_keys[RotationIndex].value;
    const glm::quat& EndRotationQ = anim.rotate_keys[NextRotationIndex].value;
    Out = glm::slerp(StartRotationQ, EndRotationQ, Factor);
}

void scn::init_animation_system(ecs::system_factory& factory)
{
    factory.register_automatic_system<update_animation_controllers_system>("scn::animation_controller_update");
    factory.register_automatic_system<update_nodes_animation_system>("scn::animation_system_node");
    factory.register_automatic_system<update_bone_offsets_system>("scn::animation_system_matrix");
}
