#include "scn_primitives.h"
#include <rnd_render_system.h>

#include <numeric>

scn::model_web scn::generate_web(glm::ivec2 size)
{
    std::vector<vertex> vex;
    std::vector<unsigned int> inc;

    const int half_x = size.x / 2;
    for (int i = -half_x; i <= half_x; ++i) {
        vertex line[2];
        line[0].position = { 1.f / half_x * i, 0.f, -1.f };
        line[1].position = { 1.f / half_x * i, 0.f,  1.f };

        vex.push_back(line[0]);
        vex.push_back(line[1]);
        inc.push_back((unsigned)vex.size() - 2);
        inc.push_back((unsigned)vex.size() - 1);
    }

    const int half_y = size.y / 2;
    for (int i = -half_y; i <= half_y; ++i) {
        vertex line[2];
        line[0].position = { 1.f, 0.f, 1.f / half_y * i };
        line[1].position = { -1.f, 0.f, 1.f / half_y * i };

        vex.push_back(line[0]);
        vex.push_back(line[1]);
        inc.push_back((unsigned)vex.size() - 2);
        inc.push_back((unsigned)vex.size() - 1);
    }

    return { vex, inc };
}

scn::model_cube scn::generate_cube()
{
    std::vector<vertex> vex = {
    {.position = {-1.f, -1.f, -1.f}, .uv = {0, 0}}, // 0
    {.position = {-1.f, 1.f, -1.f}, .uv = {0, 1}}, // 1
    {.position = {1.f, 1.f, -1.f}, .uv = {1, 1}}, // 2
    {.position = {1.f, -1.f, -1.f}, .uv = {1, 0}}, // 3

    {.position = {-1.f, -1.f, 1.f}, .uv = {0, 0}}, // 4
    {.position = {1.f, -1.f, 1.f}, .uv = {1, 0}}, // 5
    {.position = {1.f, 1.f, 1.f}, .uv = {1, 1}}, // 6
    {.position = {-1.f, 1.f, 1.f}, .uv = {0, 1}}  // 7
    };

    std::vector<unsigned int> inc = {
        0, 1, 3,
        3, 1, 2,

        4, 5, 7,
        7, 5, 6,

        0, 4, 1,
        1, 4, 7,

        2, 6, 3,
        3, 6, 5,

        1, 7, 2,
        2, 7, 6,

        0, 3, 4,
        4, 3, 5
    };
    //std::vector<res::Vertex> vex
    //{
    //    {.position = {-1.f, -1.f, -1.f}, .uv = {0,0}}, // 0
    //    {.position = { -1.f, 1.f, -1.f}, .uv = {0,1}}, // 1
    //    {.position = { 1.f,  1.f, -1.f}, .uv = {1,1}}, // 2
    //    {.position = { 1.f, -1.f, -1.f}, .uv = {1,0}}, // 3

    //    {.position = {-1.f, -1.f, 1.f}, .uv = {1,0}},  // 4
    //    {.position = { 1.f, -1.f, 1.f}, .uv = {0,0}},  // 5
    //    {.position = { 1.f,  1.f, 1.f}, .uv = {0,1}},  // 6
    //    {.position = {-1.f,  1.f, 1.f}, .uv = {1,1}},  // 7

    //    {.position = {-1.f, -1.f,-1.f}, .uv = {0,0}},  // 8
    //    {.position = {-1.f, -1.f, 1.f}, .uv = {1,0}},  // 9
    //    {.position = {-1.f,  1.f, 1.f}, .uv = {1,1}},  // 10
    //    {.position = {-1.f,  1.f,-1.f}, .uv = {0,1}},  // 11

    //    {.position = { 1.f, -1.f, -1.f}, .uv = {1,0}}, // 12
    //    {.position = { 1.f,  1.f, -1.f}, .uv = {0,0}}, // 13
    //    {.position = { 1.f,  1.f,  1.f}, .uv = {0,1}}, // 14
    //    {.position = { 1.f,  -1.f, 1.f}, .uv = {1,1}}, // 15

    //    {.position = {-1.f, -1.f, -1.f}, .uv = {0,0}}, // 16
    //    {.position = { 1.f, -1.f, -1.f}, .uv = {1,0}}, // 17
    //    {.position = { 1.f, -1.f,  1.f}, .uv = {1,1}}, // 18
    //    {.position = {-1.f, -1.f,  1.f}, .uv = {0,1}}, // 19

    //    {.position = {-1.f,  1.f, -1.f}, .uv = {0,0}}, // 20
    //    {.position = {-1.f,  1.f,  1.f}, .uv = {1,0}}, // 21
    //    {.position = { 1.f,  1.f,  1.f}, .uv = {1,1}}, // 22
    //    {.position = { 1.f,  1.f, -1.f}, .uv = {0,1}}, // 23
    //};
    //std::vector<unsigned int> inc
    //{
    //    /*
    //        0 --- 1
    //        |    /|
    //        |  /  |
    //        |/    |
    //        3 --- 2
    //    */
    //    0,1,3,
    //    3,1,2,

    //    4,5,6,
    //    6,5,7,

    //    8,9,11,
    //    11,9,10,

    //    12,13,15,
    //    15,13,14,

    //    16,17,19,
    //    19,17,18,

    //    20,21,23,
    //    23,21,22,
    //};

    for (int i = 0; i < inc.size(); i += 3) {
        auto& p1 = vex[inc[i]];
        auto& p2 = vex[inc[i + 1]];
        auto& p3 = vex[inc[i + 2]];

        p1.normal = glm::normalize(glm::cross(p3.position - p2.position, p1.position - p2.position));
        p2.normal = p1.normal;
        p3.normal = p1.normal;
    }

    return { vex, inc };
}

std::vector<scn::vertex> generate_sphere_data(float radius, float sectorCount, float stackCount)
{
    std::vector<scn::vertex> result;

    constexpr float PI = (float)std::numbers::pi;
    float x, y, z, xy;                              // vertex position
    float nx, ny, nz, lengthInv = 1.0f / radius;    // vertex normal
    float s, t;                                     // vertex texCoord

    float sectorStep = 2 * PI / sectorCount;
    float stackStep = PI / stackCount;

    for (int i = 0; i <= stackCount; ++i)
    {
        const float stackAngle = PI / 2 - i * stackStep;        // starting from pi/2 to -pi/2
        xy = radius * std::cos(stackAngle);             // r * cos(u)
        z = radius * std::sin(stackAngle);              // r * sin(u)

        // add (sectorCount+1) vertices per stack
        // first and last vertices have same position and normal, but different tex coords
        for (int j = 0; j <= sectorCount; ++j)
        {
            const float sectorAngle = j * sectorStep;           // starting from 0 to 2pi

            scn::vertex vert;

            // vertex position (x, y, z)
            x = xy * std::cos(sectorAngle);             // r * cos(u) * cos(v)
            y = xy * std::sin(sectorAngle);             // r * cos(u) * sin(v)
            vert.position = glm::vec3(x, y, z);

            // normalized vertex normal (nx, ny, nz)
            nx = x * lengthInv;
            ny = y * lengthInv;
            nz = z * lengthInv;
            vert.normal = glm::vec3(nx, ny, nz);

            // vertex tex coord (s, t) range between [0, 1]
            s = (float)j / sectorCount;
            t = (float)i / stackCount;
            vert.uv = glm::vec2(s, t);
            result.push_back(vert);
        }
    }

    return result;
}

scn::model_sphere scn::generate_sphere()
{
    float radius = 1.0f;
    float sectorCount = 72.f;
    float stackCount = 24.f;

    auto vex = generate_sphere_data(radius, sectorCount, stackCount);

    // generate CCW index list of sphere triangles
    // k1--k1+1
    // |  / |
    // | /  |
    // k2--k2+1
    std::vector<unsigned int> inc;
    std::vector<int> lineIndices;

    for (int i = 0; i < stackCount; ++i)
    {
        int k1 = i * (int)(sectorCount + 1);     // beginning of current stack
        int k2 = k1 + (int)sectorCount + 1;      // beginning of next stack

        for (int j = 0; j < (int)sectorCount; ++j, ++k1, ++k2)
        {
            // 2 triangles per sector excluding first and last stacks
            // k1 => k2 => k1+1
            if (i != 0)
            {
                inc.push_back(k1);
                inc.push_back(k2);
                inc.push_back(k1 + 1);
            }

            // k1+1 => k2 => k2+1
            if (i != (stackCount - 1))
            {
                inc.push_back(k1 + 1);
                inc.push_back(k2);
                inc.push_back(k2 + 1);
            }

            // store indices for lines
            // vertical lines for all stacks, k1 => k2
            lineIndices.push_back(k1);
            lineIndices.push_back(k2);
            if (i != 0)  // horizontal lines except 1st stack, k1 => k+1
            {
                lineIndices.push_back(k1);
                lineIndices.push_back(k1 + 1);
            }
        }
    }

    for (auto& v : vex) {
        v.uv = v.uv / glm::vec2(16, -16);
    }

    return { vex, inc };
}

#include <execution>
namespace {
    struct is_local_transform_invalidated {};
    struct is_transform_will_update_by_parent {};

    void calc_world_transforms(ecs::entity ent, entt::registry & registry)
    {
        glm::mat4 local = glm::mat4{ 1.0 };
        glm::mat4 parent = glm::mat4{ 1.0 };

        if (registry.all_of<scn::parent_component>(ent))
        {
            auto& parent_ = registry.get<scn::parent_component>(ent);
            if (registry.all_of<scn::world_transform>(parent_.parent))
            {
                auto& parent_trans = registry.get<scn::world_transform>(parent_.parent);
                parent = parent_trans.world;
            }
        }

        if (registry.all_of<scn::local_transform>(ent))
        {
            auto& trans = registry.get<scn::local_transform>(ent);
            local = trans.local;
        }
        registry.emplace_or_replace<scn::world_transform>(ent, parent * local);

        registry.remove<is_local_transform_invalidated>(ent);
        registry.remove<is_transform_will_update_by_parent>(ent);

        if (registry.all_of<scn::children_component>(ent))
        {
            auto& children = registry.get<scn::children_component>(ent);
            for (auto& child : children.children)
            {
                calc_world_transforms(child, registry);
            }
        }
    }

    void update_transform_system(entt::registry & registry)
    {
        auto entts = registry.view<is_local_transform_invalidated>();
        std::for_each(std::execution::unseq, entts.begin(), entts.end(), [&](auto ent) { calc_world_transforms(ent, registry); });
    }

    void on_validate_local_transform(entt::registry & registry, ecs::entity ent)
    {

        if (registry.all_of<is_transform_will_update_by_parent>(ent)) {
            return;
        }

        registry.remove<is_local_transform_invalidated>(ent);
        registry.emplace<is_transform_will_update_by_parent>(ent);
        if (registry.all_of<scn::children_component>(ent))
        {
            auto& children = registry.get<scn::children_component>(ent).children;
            for (auto& child : children)
            {
                on_validate_local_transform(registry, child);
            }
        }
    }

    void on_invalidate_local_transform(entt::registry & registry, ecs::entity ent)
    {
        if (registry.any_of<is_transform_will_update_by_parent, is_local_transform_invalidated>(ent)) {
            return;
        }

        registry.emplace<is_local_transform_invalidated>(ent);
        if (registry.all_of<scn::children_component>(ent))
        {
            auto& children = registry.get<scn::children_component>(ent).children;
            for (auto& child : children)
            {
                on_validate_local_transform(registry, child);
            }
        }
    }
}


namespace {
    void calc_interpolated_scaling(glm::vec3& Out, float AnimationTimeTicks, const scn::animation_node& anim);
    void calc_interpolated_position(glm::vec3& Out, float AnimationTimeTicks, const scn::animation_node& anim);
    void calc_interpolated_rotation(glm::quat& Out, float AnimationTimeTicks, const scn::animation_node& anim);

    void update_bone_offsets_system(entt::registry& registry)
    {
        auto view = registry.view<scn::bone_component, scn::world_transform, scn::obj_owner_component>();
        for (const auto& [ent, bone, transform, owner] : view.each())
        {
            auto& obj = owner.owner;
            auto& matrices = registry.get<scn::bone_matrices_component>(obj);
            if (bone.index >= 0 && bone.index < (int)matrices.matrices.size())
            {
                matrices.matrices[bone.index] = transform.world * bone.offset;
            }
        }
    }

    void update_nodes_animation_system(entt::registry& registry, const scn::delta_time& dt)
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
                }
                else {
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
}


#include "scn_camera_controller_component.hpp"
#include "inp_input_system.h"

namespace
{
    struct is_mouse_controller_updated {};
    void on_update_mouse_controller_by_mouse_click_event(entt::registry& registry, ecs::entity evt) {
        const auto& mouse_btn = registry.get<inp::mouse_click_event>(evt);

        for (const auto& [ent, data] : registry.view<scn::mouse_controller_component>().each()) {
            auto& is_moving = data.is_moving;
            auto& is_rotating = data.is_rotating;
            if (mouse_btn.key == inp::MOUSE_BUTTONS::LEFT)
                is_moving = (mouse_btn.action == inp::KEY_ACTION::DOWN);
            if (mouse_btn.key == inp::MOUSE_BUTTONS::RIGHT)
                is_rotating = (mouse_btn.action == inp::KEY_ACTION::DOWN);
            registry.emplace_or_replace<is_mouse_controller_updated>(ent);
        }
    }

    void on_update_mouse_controller_by_mouse_move_event(entt::registry& registry, ecs::entity ent) {
        const auto& cursor = registry.get<inp::cursor_move_event>(ent);

        for (const auto& [ent, data] : registry.view<scn::mouse_controller_component>().each()) {
            auto& is_moving = data.is_moving;
            auto& is_rotating = data.is_rotating;
            auto& last_mouse_pos = data.last_mouse_pos;
            auto& pos = data.position;
            auto& rotation = data.rotation;
            auto& speed = data.movement_speed;
            auto& rotate_speed = data.rotating_speed;
            glm::vec2 delta = cursor.direction;
            last_mouse_pos = cursor.pos;

            if (is_moving)
            {
                glm::vec3 addition = glm::vec3(delta.x, 0, delta.y);
                pos += (glm::rotateY(addition * speed * 3, rotation.y));
            }

            if (is_rotating)
            {
                float pitch = delta.y * rotate_speed;
                float yaw = delta.x * rotate_speed;

                rotation.x = std::clamp(rotation.x + pitch, -glm::radians(90.0f), glm::radians(90.0f));
                rotation.y += yaw;
            }
            registry.emplace_or_replace<is_mouse_controller_updated>(ent);
        }
    }

    void on_update_mouse_controller_by_scroll_event(entt::registry& registry, ecs::entity ent) {
        const auto& scroll = registry.get<inp::scroll_move_event>(ent);

        for (const auto& [ent, data] : registry.view<scn::mouse_controller_component>().each()) {
            auto& distance = data.distance;
            auto& speed = data.movement_speed;
            distance -= scroll.direction.y * speed;
            distance = std::clamp(distance, 0.f, 150.f);
            registry.emplace_or_replace<is_mouse_controller_updated>(ent);
        }
    }

    void on_update_mouse_controller_local_transform(entt::registry& registry, ecs::entity ent) {
        if (registry.all_of<scn::local_transform, scn::mouse_controller_component>(ent)) {
            auto& trans = registry.get<scn::local_transform>(ent);
            auto& data = registry.get<scn::mouse_controller_component>(ent);
            auto& rotation = data.rotation;
            auto& pos = data.position;
            auto& distance = data.distance;
            glm::mat4 orientation = glm::toMat4(glm::quat(rotation));
            trans.local = glm::translate(pos) * orientation * glm::translate(glm::mat4(1.0), glm::vec3(0, 0, distance));
        }
    }
}

#include "ecs_component.h"

void clear_input_events(entt::registry& registry)
{
    const auto ents = registry.view<ecs::input_changed_event_component>();
    registry.destroy(ents.begin(), ents.end());
}

void registate_systems_world(scn::world& world)
{
    entt::sigh_helper{ world.state() }
        .with<inp::mouse_click_event>()
        .on_construct<on_update_mouse_controller_by_mouse_click_event>()
        .on_update<on_update_mouse_controller_by_mouse_click_event>()
        .with<inp::cursor_move_event>()
        .on_construct<on_update_mouse_controller_by_mouse_move_event>()
        .on_update<on_update_mouse_controller_by_mouse_move_event>()
        .with<inp::scroll_move_event>()
        .on_construct<on_update_mouse_controller_by_scroll_event>()
        .on_update<on_update_mouse_controller_by_scroll_event>()
        .with<is_mouse_controller_updated>()
        .on_construct<on_update_mouse_controller_local_transform>()
        .on_update<on_update_mouse_controller_local_transform>();

    entt::sigh_helper{ world.state() }
        .with<scn::local_transform>()
        .on_construct<on_invalidate_local_transform>()
        .on_update<on_invalidate_local_transform>();

    world.organizer().emplace<update_transform_system>("update_transform_system");

    world.organizer().emplace<update_bone_offsets_system>("update_bone_offsets_system");
    world.organizer().emplace<update_nodes_animation_system>("update_node_animation_system");

    world.organizer().emplace<&clear_input_events>("clear input events");
}