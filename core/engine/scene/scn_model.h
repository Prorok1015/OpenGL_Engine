#pragma once
#include <common.h>
#include <texture/rnd_texture_manager.h>
#include "ecs_entity.h"
#include "scn_primitives.h"
#include "scn_material_desc.h"
#include "scn_mesh_nodes.hpp"
#include "ecs_event.hpp"

namespace scn {

    struct delta_time { float dt; };
    struct fixed_time { float dt; };
    struct update_alpha { float dt; };

    struct transform_updated {};
    struct hierarchy_updated {};

    struct depth_level {
        std::uint32_t value = 0;
    };

    struct material_desc_component {
        res::res_handle<scn::material_desc> mlt_desc;
    };

    struct name_component {
        std::string name;
    };

    struct mesh_component {
        scn::mesh_view mesh;
    };

    struct hightlight_component {
        ds::color color = ds::color(0.0f, 1.0f, 0.0f, 1.0f);
		std::vector<uint32_t> triangles;
	};

	struct geometry_component {
		res::tag geom_tag;
	};

    struct animation_controller_component {
        std::string current_animation;
        float current_time = 0.f;
        float speed = 1.f;
        bool playing = false;
        bool loop = true;
        // Cached from animation metadata when animation is selected:
        float duration = 0.f;
        float ticks_per_second = 25.f;
    };

	struct object_component {
	};

    struct animated_node_component {
        std::string node_name;
    };

    struct bone_component {
        glm::mat4 offset{ 1.0 };
        int index = -1;
    };

    struct skeleton_component {
        res::tag skinning_tag;
        int bone_count = 0;
        bool show_skeleton = false;
    };

    struct skinning_component {
        res::tag skinning_tag;
        entt::entity skeleton_entity = entt::null;
    };

    struct bone_matrices_component {
        std::vector<glm::mat4> matrices;
	};

    struct parent_component {
        entt::entity parent;
    };

    struct children_component {
        std::vector<entt::entity> children;
    };

    struct local_transform {
        glm::mat4 local = glm::mat4{ 1.0 };
        
        void set(entt::entity ent, const glm::mat4& matr, ecs::event<scn::transform_updated>& event) {
            local = matr;
            event.emit(ent);
        }
    };

    struct world_transform {
        glm::mat4 world = glm::mat4{ 1.0 };
    };

    struct scene_anchor_component {

    };

    struct renderable {

    };

    struct sky_component
    {
        std::vector<res::tag> cube_map;
    };

    struct directional_light
    {
        glm::vec4 direction;
        glm::vec4 diffuse;
        glm::vec4 ambient;
        glm::vec4 specular;
    };
}