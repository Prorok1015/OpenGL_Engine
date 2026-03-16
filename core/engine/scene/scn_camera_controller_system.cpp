#include "scn_camera_controller_system.h"
#include "scn_camera_controller_component.hpp"
#include "scn_model.h"
#include "inp_input_system.h"
#include "ecs_event.hpp"

void update_camera_matrix_system(const inp::input_state& state,
                                 ecs::event<scn::transform_updated>& event,
                                 entt::view<entt::get_t<scn::local_transform, scn::mouse_controller_component>> view)
{
    for (auto&& [ent, local, controller] : view.each()) {
        auto& rotation = controller.rotation;
        auto& pos = controller.position;
        auto& last_mouse_pos = controller.last_mouse_pos;
        auto& distance = controller.distance;
        auto& speed = controller.movement_speed;
        auto& is_moving = controller.is_moving;
        auto& is_rotating = controller.is_rotating;
        auto& rotate_speed = controller.rotating_speed;

        is_moving = state.mouse[(int)inp::MOUSE_BUTTONS::LEFT];
        is_rotating = state.mouse[(int)inp::MOUSE_BUTTONS::RIGHT];

        glm::vec2 delta = state.mouse_dir;
        last_mouse_pos = state.mouse_pos;

        if (is_moving) {
            glm::vec3 addition = glm::vec3(delta.x, 0, delta.y);
            pos += (glm::rotateY(addition * speed, rotation.y));
        }

        if (is_rotating) {
            float pitch = delta.y * rotate_speed;
            float yaw = delta.x * rotate_speed;

            rotation.x = std::clamp(rotation.x + pitch, -glm::radians(90.0f), glm::radians(90.0f));
            rotation.y += yaw;
        }

        distance -= state.scroll.y * speed;
        distance = std::clamp(distance, 0.f, 150.f);


        glm::mat4 orientation = glm::toMat4(glm::quat(rotation));
        local.local = glm::translate(pos) * orientation * glm::translate(glm::mat4(1.0), glm::vec3(0, 0, distance));
        if (is_moving || is_rotating || state.scroll.y > 0)
            event.emit(ent);
    }
}

void clear_input_state_system(inp::input_state& state)
{
    state.scroll = glm::vec2{};
    state.mouse_dir = glm::vec2{};
}

void scn::init_mouse_controller_system(ecs::system_factory& factory)
{
    factory.register_automatic_system<update_camera_matrix_system>("scn::update_camera_matrix_system");
    factory.register_automatic_system<clear_input_state_system>("inp::clear_input_state");
}