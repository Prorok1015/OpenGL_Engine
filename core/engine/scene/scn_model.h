#pragma once
#include <common.h>
#include <texture/rnd_texture_manager.h>
#include "ecs_entity.h"
#include "scn_primitives.h"
#include "scn_material_desc.h"
#include <res_mesh.hpp>

namespace scn {

    struct delta_time { float dt; };

    struct material_desc_component {
        std::shared_ptr<scn::material_desc> mlt_desc;
    };

    struct keyframes_component {
        std::unordered_map<std::string, res::animation_node> keyframes;
    };

    struct name_component {
        std::string name;
    };

    struct mesh_component {
        res::mesh_view mesh;
    };

    struct hightlight_component {
        ds::color color = ds::color(1.0f, 0.0f, 0.0f, 1.0f);
		std::vector<uint32_t> triangles;
	};

	struct geometry_component {
		res::tag geom_tag;
	};

    struct playable_animation_component {
        std::string name;
		float duration = 0.f;
		float ticks_per_second = 0.f;
        float current_tick = 0.f;
        bool is_repeat_animation = true;
    };

    struct animations_component {
        std::vector<res::animation> animations;
    };

	struct obj_owner_component {
		ecs::entity owner;
	};

	struct object_component {
	};

    struct bone_component {
        glm::mat4 offset{ 1.0 };
        int index = -1;
    };

    struct skinning_component {
        res::tag skinning_tag;
    };

    struct bone_matrices_component {
        std::vector<glm::mat4> matrices;
	};

    struct parent_component {
        ecs::entity parent;
    };

    struct children_component {
        std::vector<ecs::entity> children;
    };

    struct local_transform {
        glm::mat4 local = glm::mat4{ 1.0 };
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