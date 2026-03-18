#include "scn_render_data_extractor.h"
#include "eng_transform_3d.hpp"
#include "rnd_light_data.hpp"
#include "rnd_render_packet.hpp"
#include "rnd_render_system.h"
#include "scn_camera_component.hpp"
#include "scn_material_desc.h"
#include "scn_model.h"
#include "eng_profiler.h"

void scn::render_data_extractor::extract(rnd::frame_context& context) {
    PROFILE_SCOPE("ExtractRenderData");
    auto& level = m_level_manager.get_level();

    std::vector<rnd::render_packet_t> packets;
    int dir_light_count = 0;

    if (context.data.has_value<rnd::scene_lights_t>()) {
        dir_light_count = static_cast<int>(context.data.require<rnd::scene_lights_t>().directional.size());
    }

    for (uint32_t i = 0; i < level.get_world_count(); ++i) {
        auto& reg = level.get_world(i).state();

        for (auto [ent, cam] : reg.view<scn::camera_component>().each()) {
            if (cam.m_viewport.size.x < 1 || cam.m_viewport.size.y < 1) {
                continue;
            }

            rnd::render_packet_t packet;

            packet.camera.view_matrix = glm::mat4{ 1.0f };
            packet.camera.view_position = glm::vec3{ 0.0f };

            if (auto* trans = reg.try_get<scn::world_transform>(ent)) {
                packet.camera.view_matrix = glm::inverse(trans->world);
                packet.camera.view_position = glm::vec3(trans->world[3]);
            }

            auto& vp = cam.m_viewport;
            packet.camera.viewport = vp;
            packet.camera.projection_matrix = glm::perspective(glm::radians(cam.fov), (float)vp.size.x / (float)vp.size.y,
                cam.near_distance, cam.far_distance);
            packet.camera.target_texture_tag = cam.texture;
            packet.camera.render_priority = 0;

            auto& tex_mgr = rnd::get_system().get_texture_manager();

            auto meshes_view = reg.view<scn::mesh_component, scn::geometry_component, scn::material_desc_component, scn::world_transform>();
            for (auto&& [m_ent, mesh, geom, mat, m_trans] : meshes_view.each()) {

                if (!mat.mlt_desc.is_ready()) {
                    continue;
                }

                auto& mlt = *mat.mlt_desc;

                rnd::draw_call_t dc;
                dc.geometry_tag = geom.geom_tag;
                dc.indices_count = static_cast<uint32_t>(mesh.mesh.get_indices_count());
                dc.vx_begin = static_cast<uint32_t>(mesh.mesh.vx_begin);
                dc.ind_begin = static_cast<uint32_t>(mesh.mesh.ind_begin);
                dc.transform = m_trans.world;

                const auto queue = mlt.queue;
                auto shader_cfg = mlt.get_shader_desc(entt::handle{ reg, m_ent }, tex_mgr);

                if (dir_light_count > 0) {
                    shader_cfg.cdata.constants["DIRECTION_LIGHT_COUNT"] = std::to_string(dir_light_count);
                }

                if (auto* owner = reg.try_get<scn::obj_owner_component>(m_ent)) {
                    auto owner_ent = owner->owner;
                    if (reg.all_of<scn::bone_matrices_component>(owner_ent) &&
                        reg.all_of<scn::skinning_component>(owner_ent)) {

                        auto& skm = reg.get<scn::skinning_component>(owner_ent);
                        auto& matrices = reg.get<scn::bone_matrices_component>(owner_ent);

                        dc.bone_matrices = matrices.matrices;
                        dc.skinning_tag = skm.skinning_tag;

                        shader_cfg.cdata.defines.push_back("USE_ANIMATION");
                    }
                }

                if (queue == scn::pass_queue::OPAQUE || queue == scn::pass_queue::MIX) {
                    rnd::draw_call_t opaque_dc = dc;
                    opaque_dc.material = shader_cfg;
                    opaque_dc.material.cdata.defines.push_back("OPAQUE");
                    opaque_dc.sort_key = opaque_dc.material.get_hash();
                    packet.opaque_draws.push_back(std::move(opaque_dc));
                }

                if (queue == scn::pass_queue::TRANSPARENT || queue == scn::pass_queue::MIX) {
                    rnd::draw_call_t trans_dc = dc;
                    trans_dc.material = std::move(shader_cfg);
                    trans_dc.sort_key = trans_dc.material.get_hash();
                    packet.transparent_draws.push_back(std::move(trans_dc));
                }
            }

            auto skys_view = reg.view<scn::sky_component, scn::material_desc_component, scn::renderable>();
            for (auto [sky_ent, sky, mat] : skys_view.each()) {
                if (mat.mlt_desc.is_ready()) {
                    packet.skybox_material = mat.mlt_desc->get_shader_desc(entt::handle{ reg, sky_ent }, tex_mgr);
                }
            }

            packets.push_back(std::move(packet));
        }
    }

    context.data.construct<std::vector<rnd::render_packet_t>>(std::move(packets));
}
