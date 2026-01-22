#include "scn_animation_job.h"
#include "scn_mesh_nodes.hpp"
#include "scn_model.h"
#include "ecs_event.hpp"

void calc_interpolated_scaling(glm::vec3& Out, float AnimationTimeTicks, const scn::animation_node& anim);
void calc_interpolated_position(glm::vec3& Out, float AnimationTimeTicks, const scn::animation_node& anim);
void calc_interpolated_rotation(glm::quat& Out, float AnimationTimeTicks, const scn::animation_node& anim);

void update_bone_offsets_system(
    entt::view<entt::get_t<const scn::bone_component, const scn::world_transform, const scn::obj_owner_component>> bones_view,
    entt::view<entt::get_t<scn::bone_matrices_component>> skeletons_view
)
{
    for (auto&& [ent, bone, transform, owner] : bones_view.each()) {
        if (skeletons_view.contains(owner.owner)) {
            auto& matrices_comp = skeletons_view.get<scn::bone_matrices_component>(owner.owner);

            if (bone.index >= 0 && bone.index < (int)matrices_comp.matrices.size()) {
                matrices_comp.matrices[bone.index] = transform.world * bone.offset;
            }
        }
    }
}

void update_nodes_animation_system(scn::level_res<const scn::delta_time> dt,
                                   entt::view<entt::get_t<const scn::keyframes_component, scn::playable_animation_component>> view,
                                   entt::view<entt::get_t<scn::local_transform>> locals,
                                   ecs::event<scn::transform_updated>& event
    )
{
    for (auto [ent, keyframes, animation] : view.each())
    {
        animation.current_tick += dt->dt;
        const float ticks_per_second = animation.ticks_per_second > 0 ? animation.ticks_per_second : 25.0f;
        const float time_in_ticks = animation.current_tick * ticks_per_second;
        float ticks = fmod(time_in_ticks, animation.duration);
        if (time_in_ticks > animation.duration) {
            if (animation.is_repeat_animation) {
                animation.current_tick = 0.f;
            } else {
                ticks = animation.duration;
                continue;
            }
        }

        auto& keys = keyframes.keyframes;
        if (auto it = keys.find(animation.name); it != keys.end()) {
            // Interpolate scaling and generate scaling transformation matrix
            glm::vec3 scaling;
            calc_interpolated_scaling(scaling, ticks, it->second);
            glm::mat4 scaling_m = glm::scale(scaling);

            // Interpolate rotation and generate rotation transformation matrix
            glm::quat rotation_q;
            calc_interpolated_rotation(rotation_q, ticks, it->second);
            glm::mat4 rotation_m = glm::toMat4(rotation_q);

            // Interpolate translation and generate translation transformation matrix
            glm::vec3 translation;
            calc_interpolated_position(translation, ticks, it->second);
            glm::mat4 translation_m = glm::translate(translation);

            // Combine the above transformations
            auto local = translation_m * rotation_m * scaling_m;
            
            locals.get<scn::local_transform>(ent).local = local;
            event.emit(ent);
        }
    }
}

template<class T>
std::size_t find_keyframe_index(const std::vector<T>& arr, float time)
{
    for (std::size_t i = 0; i < arr.size() - 1; i++) {
        float t = (float)arr[i + 1].time;
        if (time < t) {
            return i;
        }
    }

    return 0;
}

void calc_interpolated_scaling(glm::vec3& Out, float AnimationTimeTicks, const scn::animation_node& anim)
{
    // we need at least two values to interpolate...
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
    // we need at least two values to interpolate...
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
    // we need at least two values to interpolate...
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
    factory.register_automatic_system<update_bone_offsets_system>("scn::animation_system_matrix");
    factory.register_automatic_system<update_nodes_animation_system>("scn::animation_system_node");
}
