#include "common.h"
#include "res_resource_model.h"
#include "res_picture.h"
#include "res_system.h"
#include "res_model_loader.h"


res::Model::Model(const Tag& tag)
	: Resource(tag)
{
	loader::model_loader ld(tag);
    meshes = ld.load();
	model = ld.model;
}

res::Model::Model(const Tag& tag, const res::model_presintation& model_)
    : Resource(tag)
    , model(model_)
{
}

std::vector<glm::mat4> res::Model::get_bone_transforms(double TimeInSeconds, std::string_view animation_name)
{
    glm::mat4 Identity{ 1.0 };

    std::size_t anim_idx = -1;
    for (std::size_t idx = 0; idx < model.animations.size(); ++idx) {
        if (animation_name == model.animations[idx].name) {
            anim_idx = idx;
            break;
        }
    }

    auto& animation = model.animations[anim_idx];

    float TicksPerSecond = (float)(animation.ticks_per_second > 0 ? animation.ticks_per_second : 25.0f);
    float TimeInTicks = TimeInSeconds * TicksPerSecond;
    float AnimationTimeTicks = fmod(TimeInTicks, (float)animation.duration);

    std::vector<glm::mat4> result(model.data.bones.size());

    calculate_bone_transform(model.head, AnimationTimeTicks, animation, Identity, result);
    
    return result;
}

void res::Model::calculate_bone_transform(res::node_hierarchy_view& node, float time_sec, const res::animation& anim, const glm::mat4& parent, std::vector<glm::mat4>& out_transform)
{
    glm::mat4 node_transform = node.mt;

    if (node.bone_id != -1)
    {
        auto& bone = model.data.bones[node.bone_id];

        if (auto it = bone.anim.find(anim.name); it != bone.anim.end()) {
            // Interpolate scaling and generate scaling transformation matrix
            glm::vec3 Scaling;
            calc_interpolated_scaling(Scaling, time_sec, it->second);
            glm::mat4 ScalingM = glm::scale(Scaling);

            // Interpolate rotation and generate rotation transformation matrix
            glm::quat RotationQ;
            calc_interpolated_rotation(RotationQ, time_sec, it->second);
            glm::mat4 RotationM = glm::toMat4(RotationQ);

            // Interpolate translation and generate translation transformation matrix
            glm::vec3 Translation;
            calc_interpolated_position(Translation, time_sec, it->second);
            glm::mat4 TranslationM = glm::translate(Translation);

            // Combine the above transformations
            node_transform = TranslationM * RotationM * ScalingM;
        }
    }

    if (!node.anim.empty()) {
        if (auto it = node.anim.find(anim.name); it != node.anim.end()) {
            // Interpolate scaling and generate scaling transformation matrix
            glm::vec3 Scaling;
            calc_interpolated_scaling(Scaling, time_sec, it->second);
            glm::mat4 ScalingM = glm::scale(Scaling);

            // Interpolate rotation and generate rotation transformation matrix
            glm::quat RotationQ;
            calc_interpolated_rotation(RotationQ, time_sec, it->second);
            glm::mat4 RotationM = glm::toMat4(RotationQ);

            // Interpolate translation and generate translation transformation matrix
            glm::vec3 Translation;
            calc_interpolated_position(Translation, time_sec, it->second);
            glm::mat4 TranslationM = glm::translate(Translation);

            // Combine the above transformations
            node_transform = TranslationM * RotationM * ScalingM;
            node.mt = node_transform;
        }
    }

    glm::mat4 GlobalTransformation = parent * node_transform;

    if (node.bone_id != -1)
    {
        auto& bone = model.data.bones[node.bone_id];
        out_transform[node.bone_id] = (glm::inverse(model.head.mt) * GlobalTransformation * bone.offset);
    }

    for (auto& child : node.children) {
        calculate_bone_transform(child, time_sec, anim, GlobalTransformation, out_transform);
    }
}

template<class T>
std::size_t find_id(const std::vector<T>& arr, float time)
{
    for (std::size_t i = 0; i < arr.size() - 1; i++) {
        float t = (float)arr[i + 1].time;
        if (time < t) {
            return i;
        }
    }

    return 0;
}


void res::Model::calc_interpolated_scaling(glm::vec3& Out, float AnimationTimeTicks, const res::animation_node& anim)
{
    // we need at least two values to interpolate...
    if (anim.scale_keys.size() == 1) {
        Out = anim.scale_keys[0].value;
        return;
    }

    std::size_t ScalingIndex = find_id(anim.scale_keys, AnimationTimeTicks);
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

void res::Model::calc_interpolated_position(glm::vec3& Out, float AnimationTimeTicks, const res::animation_node& anim)
{
    // we need at least two values to interpolate...
    if (anim.pos_keys.size() == 1) {
        Out = anim.pos_keys[0].value;
        return;
    }

    std::size_t PositionIndex = find_id(anim.pos_keys, AnimationTimeTicks);
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

void res::Model::calc_interpolated_rotation(glm::quat& Out, float AnimationTimeTicks, const res::animation_node& anim)
{
    // we need at least two values to interpolate...
    if (anim.rotate_keys.size() == 1) {
        Out = anim.rotate_keys[0].value;
        return;
    }

    std::size_t RotationIndex = find_id(anim.rotate_keys, AnimationTimeTicks);
    std::size_t NextRotationIndex = RotationIndex + 1;

    float t1 = (float)anim.rotate_keys[RotationIndex].time;
    float t2 = (float)anim.rotate_keys[NextRotationIndex].time;
    float DeltaTime = t2 - t1;
    float Factor = std::clamp((AnimationTimeTicks - t1) / DeltaTime, 0.0f, 1.f);

    const glm::quat& StartRotationQ = anim.rotate_keys[RotationIndex].value;
    const glm::quat& EndRotationQ = anim.rotate_keys[NextRotationIndex].value;
    //aiQuaternion::Interpolate(Out, StartRotationQ, EndRotationQ, Factor);
    glm::mat4 matrix = glm::interpolate(glm::toMat4(StartRotationQ), glm::toMat4(EndRotationQ), Factor);
    
    glm::vec3 s;
    glm::vec3 po;
    glm::quat o;
    glm::vec3 sk;
    glm::vec4 p;
    glm::decompose(matrix, s, o, po, sk, p);
    Out = glm::eulerAngles(o);
    //Out = glm::normalize(Out);
}