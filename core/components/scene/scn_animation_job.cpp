#include "scn_animation_job.h"
#include "res_mesh.hpp"
#include "scn_model.h"

void calc_interpolated_scaling(glm::vec3& Out, float AnimationTimeTicks, const res::animation_node& anim);
void calc_interpolated_position(glm::vec3& Out, float AnimationTimeTicks, const res::animation_node& anim);
void calc_interpolated_rotation(glm::quat& Out, float AnimationTimeTicks, const res::animation_node& anim);

void scn::animation_job::init(entt::organizer& organizer, entt::registry& registry)
{
    organizer.emplace<&animation_job::update_bone_offsets_system>(*this, "update_bone_offsets_system");
	organizer.emplace<&animation_job::update_animation_system>(*this, "update_animation_system");
	organizer.emplace<&animation_job::update_nodes_animation_system>(*this, "update_node_animation_system");
}

void scn::animation_job::deinit(entt::organizer& organizer, entt::registry& registry)
{
}

//old
void scn::animation_job::update_bone_offsets_system(entt::registry& registry)
{
    for (ecs::entity ent : registry.view<scn::model_root_component, scn::animations_component, scn::playable_animation>())
    {
        auto& root = registry.get<scn::model_root_component>(ent);
        root.data.bones_matrices.clear();
        load_bone_offsets(ent, root.data.bones_matrices, registry);
    }

    // new
    for (entt::entity ent : registry.view<scn::bone_component, scn::world_transform, scn::obj_owner_component>())
    {
        auto& bone = registry.get<scn::bone_component>(ent);
		auto& obj = registry.get<scn::obj_owner_component>(ent).owner;
        auto& matrices = registry.get<scn::bone_matrices_component>(obj);
        if (bone.index >= 0 && bone.index < (int)matrices.matrices.size())
        {
            matrices.matrices[bone.index] = registry.get<scn::world_transform>(ent).world * bone.offset;
		}
    }
}
//old
void scn::animation_job::load_bone_offsets(ecs::entity ent, std::vector<glm::mat4>& out, entt::registry& registry)
{
    if (registry.all_of<scn::bone_component, scn::world_transform>(ent))
    {
        auto& bone = registry.get<scn::bone_component>(ent);
        auto& trans = registry.get<scn::world_transform>(ent);
        out.push_back(trans.world * bone.offset);


    }
    if (registry.all_of<scn::children_component>(ent))
    {
        auto& children = registry.get<scn::children_component>(ent).children;
        for (auto& child : children)
        {
            load_bone_offsets(child, out, registry);
        }
    }
}
// old
void scn::animation_job::update_animation_system(scn::delta_time dt, entt::registry& registry)
{
    for (ecs::entity ent : registry.view<scn::model_root_component, scn::animations_component, scn::playable_animation>())
    {
        auto& root = registry.get<scn::model_root_component>(ent);
        auto& anims = registry.get<scn::animations_component>(ent).animations;
        auto& play = registry.get<scn::playable_animation>(ent);

        auto it = std::find_if(anims.begin(), anims.end(), [&name = play.name](auto& anim) {
            return anim.name == name;
            });

        if (it != anims.end())
        {
            /*if (auto* name = ecs::registry.try_get<scn::name_component>(ent)) {
                egLOG("", "anim update started with: {}", name->name);
            } else {
                egLOG("", "anim update started with: UNNEMED");
            }*/

            auto& playable_animation = *it;
            play.current_tick += dt.dt;
            const float ticks_per_second = playable_animation.ticks_per_second > 0 ? playable_animation.ticks_per_second : 25.0f;
            const float time_in_ticks = play.current_tick * ticks_per_second;
            float animation_time_ticks = fmod(time_in_ticks, (float)playable_animation.duration);
            if (time_in_ticks > playable_animation.duration) {
                if (play.is_repeat_animation) {
                    play.current_tick = 0.f;
                }
                else {
                    animation_time_ticks = playable_animation.duration;
                }
            }

            std::vector<glm::mat4> result;
            calc_world_transforms(registry, ent, animation_time_ticks, playable_animation, result);

        }
    }
}
//old
void scn::animation_job::calc_world_transforms(entt::registry& registry ,ecs::entity ent, const float ticks, const res::animation& anim, std::vector<glm::mat4>& out)
{
    if (registry.all_of<scn::keyframes_component>(ent))
    {
        auto& keys_component = registry.get<scn::keyframes_component>(ent);
        auto& keys = keys_component.keyframes;
        if (auto it = keys.find(anim.name); it != keys.end()) {
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
            registry.emplace_or_replace<scn::local_transform>(ent, local);
        }
    }

    if (registry.all_of<scn::children_component>(ent))
    {
        auto& children = registry.get<scn::children_component>(ent).children;
        for (auto& child : children)
        {
            calc_world_transforms(registry, child, ticks, anim, out);
        }
    }
}

void scn::animation_job::update_nodes_animation_system(entt::registry& registry, const scn::delta_time& dt)
{
    for (auto [ent, keyframes, animation] : registry.view<scn::keyframes_component, scn::playable_animation_component>().each())
    {
        animation.current_tick += dt.dt;
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
            registry.emplace_or_replace<scn::local_transform>(ent, local);
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


void calc_interpolated_scaling(glm::vec3& Out, float AnimationTimeTicks, const res::animation_node& anim)
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

void calc_interpolated_position(glm::vec3& Out, float AnimationTimeTicks, const res::animation_node& anim)
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

void calc_interpolated_rotation(glm::quat& Out, float AnimationTimeTicks, const res::animation_node& anim)
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